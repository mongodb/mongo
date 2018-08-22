/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/db/single_transaction_stats.h"
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
                 unsigned long long curTime,
                 Date_t expireDate);

    /**
     * Updates relevant metrics when a storage timestamp is chosen for a transaction.
     */
    void onChooseReadTimestamp(Timestamp readTimestamp);

    /**
     * Updates relevant metrics when a transaction stashes its resources.
     */
    void onStash(ServerTransactionsMetrics* serverTransactionMetrics, unsigned long long curTime);

    /**
     * Updates relevant metrics when a transaction unstashes its resources.
     */
    void onUnstash(ServerTransactionsMetrics* serverTransactionsMetrics,
                   unsigned long long curTime);

    /**
     * Updates relevant metrics when a transaction commits.
     */
    void onCommit(ServerTransactionsMetrics* serverTransactionsMetrics,
                  unsigned long long curTime,
                  Top* top);

    /**
     * Updates relevant metrics when an active transaction aborts.
     */
    void onAbortActive(ServerTransactionsMetrics* serverTransactionsMetrics,
                       unsigned long long curTime,
                       Top* top);

    /**
     * Updates relevant metrics when an inactive transaction aborts.
     */
    void onAbortInactive(ServerTransactionsMetrics* serverTransactionsMetrics,
                         unsigned long long curTime,
                         Top* top);

    /**
     * Updates relevant metrics when an operation running on the transaction completes. An operation
     * may be a read/write operation, or an abort/commit command.
     */
    void onTransactionOperation(Client* client, OpDebug::AdditiveMetrics additiveMetrics);

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
    // Updates relevant metrics for any generic transaction abort.
    void _onAbort(ServerTransactionsMetrics* serverTransactionsMetrics,
                  unsigned long long curTime,
                  Top* top);

    // Tracks metrics for a single multi-document transaction.
    SingleTransactionStats _singleTransactionStats;
};

}  // namespace mongo
