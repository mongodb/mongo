/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/s/router_transactions_stats_gen.h"
#include "mongo/s/transaction_router.h"

namespace mongo {

/**
 * Container for router-wide multi-document transaction statistics.
 */
class RouterTransactionsMetrics {
    RouterTransactionsMetrics(const RouterTransactionsMetrics&) = delete;
    RouterTransactionsMetrics& operator=(const RouterTransactionsMetrics&) = delete;

public:
    // Cumulative metrics for a particular type of commit that a router can use.
    struct CommitStats {
        // Total number of times this commit was started.
        AtomicWord<std::int64_t> initiated{0};

        // Total number of times this commit completed successfully.
        AtomicWord<std::int64_t> successful{0};
    };

    RouterTransactionsMetrics() = default;

    static RouterTransactionsMetrics* get(ServiceContext* service);
    static RouterTransactionsMetrics* get(OperationContext* opCtx);

    std::int64_t getTotalStarted();
    void incrementTotalStarted();

    std::int64_t getTotalAborted();
    void incrementTotalAborted();

    std::int64_t getTotalCommitted();
    void incrementTotalCommitted();

    std::int64_t getTotalContactedParticipants();
    void incrementTotalContactedParticipants();

    std::int64_t getTotalParticipantsAtCommit();
    void addToTotalParticipantsAtCommit(std::int64_t inc);

    std::int64_t getTotalRequestsTargeted();
    void incrementTotalRequestsTargeted();

    const CommitStats& getCommitTypeStats_forTest(TransactionRouter::CommitType commitType);
    void incrementCommitInitiated(TransactionRouter::CommitType commitType);
    void incrementCommitSuccessful(TransactionRouter::CommitType commitType);

    void incrementAbortCauseMap(std::string abortCause);

    /**
     * Appends the accumulated stats to a sharded transactions stats object for reporting.
     */
    void updateStats(RouterTransactionsStats* stats);

private:
    /**
     * Helper to convert a CommitStats struct into the CommitTypeStats object expected by the IDL
     * RouterTransactionsStats class.
     */
    CommitTypeStats _constructCommitTypeStats(const CommitStats& stats);

    // The total number of multi-document transactions started since the last server startup.
    AtomicWord<std::int64_t> _totalStarted{0};

    // The total number of multi-document transactions that were committed through this router.
    AtomicWord<std::int64_t> _totalCommitted{0};

    // The total number of multi-document transaction that were aborted by this router.
    AtomicWord<std::int64_t> _totalAborted{0};

    // Total number of shard participants contacted over the course of any transaction, including
    // shards that may not be included in a final participant list.
    AtomicWord<std::int64_t> _totalContactedParticipants{0};

    // Total number of shard participants included in a participant list at commit.
    AtomicWord<std::int64_t> _totalParticipantsAtCommit{0};

    // Total number of network requests targeted by mongos as part of a transaction.
    AtomicWord<std::int64_t> _totalRequestsTargeted{0};

    // Structs with metrics for each type of commit a router can use.
    CommitStats _noShardsCommitStats;
    CommitStats _singleShardCommitStats;
    CommitStats _singleWriteShardCommitStats;
    CommitStats _readOnlyCommitStats;
    CommitStats _twoPhaseCommitStats;
    CommitStats _recoverWithTokenCommitStats;

    // Mutual exclusion for _abortCauseMap
    stdx::mutex _abortCauseMutex;

    // Map tracking the total number of each abort cause for any multi-statement transaction that
    // was aborted through this router.
    std::map<std::string, std::int64_t> _abortCauseMap;
};

}  // namespace mongo
