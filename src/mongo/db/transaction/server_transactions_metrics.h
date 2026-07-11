// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transaction/transactions_stats_gen.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <mutex>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Container for server-wide multi-document transaction statistics.
 */
class ServerTransactionsMetrics {
    ServerTransactionsMetrics(const ServerTransactionsMetrics&) = delete;
    ServerTransactionsMetrics& operator=(const ServerTransactionsMetrics&) = delete;

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

    void updateLastTransaction(size_t operationCount,
                               size_t oplogOperationBytes,
                               BSONObj writeConcern);

    void incrementReclaimedPreparedTxnsCommitted();
    long long getReclaimedPreparedTxnsCommitted() const;

    void incrementReclaimedPreparedTxnsAborted();
    long long getReclaimedPreparedTxnsAborted() const;

    /**
     * Appends the accumulated stats to a transactions stats object.
     * Include the 'lastCommittedTransactions' field if 'includeLastCommitted' is true.
     */
    void updateStats(TransactionsStats* stats, bool includeLastCommitted);

private:
    // The number of multi-document transactions currently active.
    Atomic<unsigned long long> _currentActive{0};

    // The number of multi-document transactions currently inactive.
    Atomic<unsigned long long> _currentInactive{0};

    // The total number of open transactions.
    Atomic<unsigned long long> _currentOpen{0};

    // The total number of multi-document transactions started since the last server startup.
    Atomic<unsigned long long> _totalStarted{0};

    // The total number of multi-document transaction aborts.
    Atomic<unsigned long long> _totalAborted{0};

    // The total number of multi-document transaction commits.
    Atomic<unsigned long long> _totalCommitted{0};

    // The total number of prepared transactions since the last server startup.
    Atomic<unsigned long long> _totalPrepared{0};

    // The total number of prepared transaction commits.
    Atomic<unsigned long long> _totalPreparedThenCommitted{0};

    // The total number of prepared transaction aborts.
    Atomic<unsigned long long> _totalPreparedThenAborted{0};

    // The current number of transactions in the prepared state.
    Atomic<unsigned long long> _currentPrepared{0};

    Atomic<long long> _reclaimedPreparedTxnsCommitted{0};
    Atomic<long long> _reclaimedPreparedTxnsAborted{0};

    // Protects member variables below.
    mutable std::mutex _mutex;

    boost::optional<LastCommittedTransaction> _lastCommittedTransaction;
};

}  // namespace mongo
