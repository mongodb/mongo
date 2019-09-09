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

#include "mongo/db/curop.h"
#include "mongo/db/server_transactions_metrics.h"
#include "mongo/db/stats/single_transaction_stats.h"
#include "mongo/db/stats/top.h"

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
    void onStash(ServerTransactionsMetrics* serverTransactionMetrics, TickSource* tickSource);

    /**
     * Updates relevant metrics when a transaction unstashes its resources.
     */
    void onUnstash(ServerTransactionsMetrics* serverTransactionsMetrics, TickSource* tickSource);

    /**
     * Updates relevant metrics when a transaction commits.
     */
    void onCommit(ServerTransactionsMetrics* serverTransactionsMetrics,
                  TickSource* tickSource,
                  Top* top);

    /**
     * Updates relevant metrics when a transaction aborts.
     * See _onAbortActive() and _onAbortInactive().
     */
    void onAbort(ServerTransactionsMetrics* serverTransactionsMetrics,
                 TickSource* tickSource,
                 Top* top);

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
                                bool isPrepared);

    /**
     * Returns a read-only reference to the SingleTransactionStats object stored in this
     * TransactionMetricsObserver instance.
     */
    const SingleTransactionStats& getSingleTransactionStats() const {
        return _singleTransactionStats;
    }

    /**
     * Resets the SingleTransactionStats object stored in this TransactionMetricsObserver instance,
     * preparing it for the new transaction or retryable write with the given number.
     */
    void resetSingleTransactionStats(TxnNumber txnNumber) {
        _singleTransactionStats = SingleTransactionStats(txnNumber);
    }

private:
    /**
     * Updates relevant metrics for any generic transaction abort.
     */
    void _onAbort(ServerTransactionsMetrics* serverTransactionsMetrics,
                  TickSource::Tick curTick,
                  TickSource* tickSource,
                  Top* top);

    /**
     * Updates relevant metrics when an active transaction aborts.
     */
    void _onAbortActive(ServerTransactionsMetrics* serverTransactionsMetrics,
                        TickSource* tickSource,
                        Top* top);

    /**
     * Updates relevant metrics when an inactive transaction aborts.
     */
    void _onAbortInactive(ServerTransactionsMetrics* serverTransactionsMetrics,
                          TickSource* tickSource,
                          Top* top);

    // Tracks metrics for a single multi-document transaction.
    SingleTransactionStats _singleTransactionStats;
};

}  // namespace mongo
