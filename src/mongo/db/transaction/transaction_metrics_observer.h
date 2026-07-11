// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/stats/single_transaction_stats.h"
#include "mongo/db/transaction/server_transactions_metrics.h"
#include "mongo/util/modules.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"

#include <cstddef>

namespace mongo {

/**
 * Updates transaction metrics (per-transaction metrics and server-wide transactions metrics) upon
 * the appropriate transaction event.
 */
class TransactionMetricsObserver {

public:
    /**
     * Updates relevant metrics when a transaction begins.
     */
    void onStart(ServerTransactionsMetrics* serverTransactionMetrics,
                 bool isAutoCommit,
                 TickSource* tickSource,
                 Date_t curWallClockTime,
                 Date_t expireDate);

    /**
     * Updates relevant metrics when a storage timestamp is chosen for a transaction.
     */
    void onChooseReadTimestamp(Timestamp readTimestamp);

    /**
     * Updates relevant metrics when a transaction stashes its resources.
     */
    void onStash(OperationContext* opCtx,
                 ServerTransactionsMetrics* serverTransactionMetrics,
                 TickSource* tickSource);

    /**
     * Updates relevant metrics when a transaction unstashes its resources.
     */
    void onUnstash(OperationContext* opCtx,
                   ServerTransactionsMetrics* serverTransactionsMetrics,
                   TickSource* tickSource);

    /**
     * Updates relevant metrics when a transaction commits.
     */
    void onCommit(OperationContext* opCtx,
                  ServerTransactionsMetrics* serverTransactionsMetrics,
                  TickSource* tickSource,
                  size_t operationCount,
                  size_t oplogOperationBytes);

    /**
     * Updates relevant metrics when a transaction aborts.
     * See _onAbortActive() and _onAbortInactive().
     */
    void onAbort(OperationContext* opCtx,
                 ServerTransactionsMetrics* serverTransactionsMetrics,
                 TickSource* tickSource);

    /**
     * Updates relevant metrics when a transcation is prepared.
     */
    void onPrepare(ServerTransactionsMetrics* serverTransactionsMetrics, TickSource::Tick curTick);

    /**
     * Updates relevant metrics and storage statistics when an operation running on the transaction
     * completes. An operation may be a read/write operation, or an abort/commit command.
     */
    void onTransactionOperation(OperationContext* opCtx,
                                OpDebug::AdditiveMetrics additiveMetrics,
                                long long prepareReadConflicts,
                                const SingleThreadedStorageMetrics& storageMetrics,
                                bool isPrepared);

    /**
     * Returns a read-only reference to the SingleTransactionStats object stored in this
     * TransactionMetricsObserver instance.
     */
    const SingleTransactionStats& getSingleTransactionStats() const {
        return _singleTransactionStats;
    }

    /**
     * Returns a reference to the SingleTransactionStats object stored in this
     * TransactionMetricsObserver instance.
     */
    SingleTransactionStats& getSingleTransactionStats() {
        return _singleTransactionStats;
    }

    /**
     * Resets the SingleTransactionStats object stored in this TransactionMetricsObserver instance,
     * preparing it for the new transaction or retryable write with the given number.
     */
    void resetSingleTransactionStats(TxnNumberAndRetryCounter txnNumberAndRetryCounter) {
        _singleTransactionStats = SingleTransactionStats(txnNumberAndRetryCounter);
    }

private:
    /**
     * Updates relevant metrics for any generic transaction abort.
     */
    void _onAbort(OperationContext* opCtx,
                  ServerTransactionsMetrics* serverTransactionsMetrics,
                  TickSource::Tick curTick,
                  TickSource* tickSource);

    /**
     * Updates relevant metrics when an active transaction aborts.
     */
    void _onAbortActive(OperationContext* opCtx,
                        ServerTransactionsMetrics* serverTransactionsMetrics,
                        TickSource* tickSource);

    /**
     * Updates relevant metrics when an inactive transaction aborts.
     */
    void _onAbortInactive(OperationContext* opCtx,
                          ServerTransactionsMetrics* serverTransactionsMetrics,
                          TickSource* tickSource);

    // Tracks metrics for a single multi-document transaction.
    SingleTransactionStats _singleTransactionStats;
};

}  // namespace mongo
