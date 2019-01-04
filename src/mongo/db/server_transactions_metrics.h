
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

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transactions_stats_gen.h"
#include "mongo/util/concurrency/with_lock.h"

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

    unsigned long long getTotalPrepared() const;
    void incrementTotalPrepared();

    unsigned long long getTotalPreparedThenCommitted() const;
    void incrementTotalPreparedThenCommitted();

    unsigned long long getTotalPreparedThenAborted() const;
    void incrementTotalPreparedThenAborted();

    unsigned long long getCurrentPrepared() const;
    void incrementCurrentPrepared();
    void decrementCurrentPrepared();

    /**
     * Returns the OpTime of the oldest oplog entry written across all open transactions.
     * Returns boost::none if there are no transaction oplog entry OpTimes stored.
     */
    boost::optional<repl::OpTime> getOldestActiveOpTime() const;

    /**
     * Add the transaction's oplog entry OpTime to oldestActiveOplogEntryOpTimes, a set of OpTimes.
     * Also creates a pair with this OpTime and OpTime::max() as the corresponding commit/abort
     * oplog entry OpTime. Finally, adds this to oldestNonMajorityCommittedOpTimes.
     */
    void addActiveOpTime(repl::OpTime oldestOplogEntryOpTime);

    /**
     * Remove the corresponding transaction oplog entry OpTime if the transaction commits or
     * aborts. Also updates the pair in oldestNonMajorityCommittedOpTimes with the
     * oldestOplogEntryOpTime to have a valid finishOpTime instead of OpTime::max(). It's stored in
     * the format: < oldestOplogEntryOpTime, finishOpTime >.
     */
    void removeActiveOpTime(repl::OpTime oldestOplogEntryOpTime,
                            boost::optional<repl::OpTime> finishOpTime);

    /**
     * Returns the number of transaction oplog entry OpTimes currently stored.
     */
    unsigned int getTotalActiveOpTimes() const;

    /**
     * Returns the oldest oplog entry OpTime across transactions whose corresponding commit or
     * abort oplog entry has not been majority committed.
     */
    boost::optional<repl::OpTime> getOldestNonMajorityCommittedOpTime() const;

    /**
     * Remove the corresponding transaction oplog entry OpTime pair from
     * oldestNonMajorityCommittedOpTimes if the transaction is majority committed or aborted.
     * We determine this by checking if there are any pairs in the set whose
     * 'finishOpTime' <= 'committedOpTime'.
     */
    void removeOpTimesLessThanOrEqToCommittedOpTime(repl::OpTime committedOpTime);

    /**
     * Testing function that adds an OpTime pair to oldestNonMajorityCommittedOpTimes.
     */
    void addNonMajCommittedOpTimePair_forTest(std::pair<repl::OpTime, repl::OpTime> OpTimePair);

    /**
     * Testing function that returns the oldest non-majority committed OpTime pair in the form:
     * < oldestOplogEntryOpTime, finishOpTime >.
     */
    boost::optional<repl::OpTime> getFinishOpTimeOfOldestNonMajCommitted_forTest() const;

    /**
     * Appends the accumulated stats to a transactions stats object.
     */
    void updateStats(TransactionsStats* stats, OperationContext* opCtx);

    /**
     * Invalidates the in-memory state of prepared transactions during replication rollback by
     * clearing oldestActiveOplogEntryOpTime, oldestActiveOplogEntryOpTimes, and
     * oldestNonMajorityCommittedOpTimes. These variables/data structures should be properly
     * reconstructed during replication recovery.
     */
    void clearOpTimes();

private:
    /**
     * Returns the first and oldest optime in the ordered set of active oplog entry optimes.
     * Returns boost::none if there are no transaction oplog entry optimes stored.
     */
    boost::optional<repl::OpTime> _calculateOldestActiveOpTime(WithLock) const;

    /**
     * Returns the oldest read timestamp in use by any open unprepared transaction. This will
     * return a null timestamp if there is no oldest open unprepared read timestamp to be
     * returned.
     */
    static Timestamp _getOldestOpenUnpreparedReadTimestamp(OperationContext* opCtx);

    //
    // Member variables, excluding atomic variables, are labeled with the following code to
    // indicate the synchronization rules for accessing them.
    //
    // (M)  Reads and writes guarded by _mutex
    //
    mutable stdx::mutex _mutex;

    // The number of multi-document transactions currently active.
    AtomicWord<unsigned long long> _currentActive{0};

    // The number of multi-document transactions currently inactive.
    AtomicWord<unsigned long long> _currentInactive{0};

    // The total number of open transactions.
    AtomicWord<unsigned long long> _currentOpen{0};

    // The total number of multi-document transactions started since the last server startup.
    AtomicWord<unsigned long long> _totalStarted{0};

    // The total number of multi-document transaction aborts.
    AtomicWord<unsigned long long> _totalAborted{0};

    // The total number of multi-document transaction commits.
    AtomicWord<unsigned long long> _totalCommitted{0};

    // The total number of prepared transactions since the last server startup.
    AtomicWord<unsigned long long> _totalPrepared{0};

    // The total number of prepared transaction commits.
    AtomicWord<unsigned long long> _totalPreparedThenCommitted{0};

    // The total number of prepared transaction aborts.
    AtomicWord<unsigned long long> _totalPreparedThenAborted{0};

    // The current number of transactions in the prepared state.
    AtomicWord<unsigned long long> _currentPrepared{0};

    // The optime of the oldest oplog entry for any active transaction.
    boost::optional<repl::OpTime> _oldestActiveOplogEntryOpTime;  // (M)

    // Maintain the oldest oplog entry OpTime across all active transactions. Currently, we only
    // write an oplog entry for an ongoing transaction if it is in the `prepare` state. By
    // maintaining an ordered set of OpTimes, the OpTime at the beginning will be the oldest.
    std::set<repl::OpTime> _oldestActiveOplogEntryOpTimes;  // (M)

    // Maintain the oldest oplog entry OpTime across transactions whose corresponding abort/commit
    // oplog entries have not been majority committed. Since this is an ordered set, the first
    // pair's oldestOplogEntryOpTime represents the earliest OpTime that we should pin the stable
    // timestamp behind.
    // Each pair is structured as follows: <oldestOplogEntryOpTime, finishOpTime>
    // 'oldestOplogEntryOpTime': The first oplog entry OpTime written by a transaction.
    // 'finishOpTime': The commit/abort oplog entry OpTime.
    // Once the corresponding abort/commit entry has been majority committed, remove the pair from
    // this set.
    std::set<std::pair<repl::OpTime, repl::OpTime>> _oldestNonMajorityCommittedOpTimes;  // (M)
};

}  // namespace mongo
