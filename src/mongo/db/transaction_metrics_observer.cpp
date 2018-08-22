/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/transaction_metrics_observer.h"

#include "mongo/db/server_transactions_metrics.h"
#include "mongo/db/transaction_participant.h"

namespace mongo {

void TransactionMetricsObserver::onStart(ServerTransactionsMetrics* serverTransactionsMetrics,
                                         bool isAutoCommit,
                                         unsigned long long curTime,
                                         Date_t expireDate) {
    //
    // Per transaction metrics.
    //
    _singleTransactionStats.setStartTime(curTime);
    _singleTransactionStats.setAutoCommit(isAutoCommit);
    _singleTransactionStats.setExpireDate(expireDate);

    //
    // Server wide transactions metrics.
    //
    serverTransactionsMetrics->incrementTotalStarted();
    serverTransactionsMetrics->incrementCurrentOpen();
    serverTransactionsMetrics->incrementCurrentInactive();
}

void TransactionMetricsObserver::onChooseReadTimestamp(Timestamp readTimestamp) {
    _singleTransactionStats.setReadTimestamp(readTimestamp);
}

void TransactionMetricsObserver::onStash(ServerTransactionsMetrics* serverTransactionsMetrics,
                                         unsigned long long curTime) {
    //
    // Per transaction metrics.
    //
    // The transaction operation may be trying to stash its resources when it has already been
    // aborted by another thread, so we check that the transaction is active before setting it as
    // inactive.
    if (_singleTransactionStats.isActive()) {
        _singleTransactionStats.setInactive(curTime);
    }

    //
    // Server wide transactions metrics.
    //
    // We accept possible slight inaccuracies in these counters from non-atomicity.
    serverTransactionsMetrics->decrementCurrentActive();
    serverTransactionsMetrics->incrementCurrentInactive();
}

void TransactionMetricsObserver::onUnstash(ServerTransactionsMetrics* serverTransactionsMetrics,
                                           unsigned long long curTime) {
    //
    // Per transaction metrics.
    //
    _singleTransactionStats.setActive(curTime);

    //
    // Server wide transactions metrics.
    //
    // We accept possible slight inaccuracies in these counters from non-atomicity.
    serverTransactionsMetrics->incrementCurrentActive();
    serverTransactionsMetrics->decrementCurrentInactive();
}

void TransactionMetricsObserver::onCommit(ServerTransactionsMetrics* serverTransactionsMetrics,
                                          unsigned long long curTime,
                                          Top* top) {
    //
    // Per transaction metrics.
    //
    // After the transaction has been committed, we must update the end time and mark it as
    // inactive. We use the same "now" time to prevent skew in the time-related metrics.
    _singleTransactionStats.setEndTime(curTime);
    // The transaction operation may have already been aborted by another thread, so we check that
    // the transaction is active before setting it as inactive.
    if (_singleTransactionStats.isActive()) {
        _singleTransactionStats.setInactive(curTime);
    }

    //
    // Server wide transactions metrics.
    //
    serverTransactionsMetrics->incrementTotalCommitted();
    serverTransactionsMetrics->decrementCurrentOpen();
    serverTransactionsMetrics->decrementCurrentActive();

    top->incrementGlobalTransactionLatencyStats(_singleTransactionStats.getDuration(curTime));
}

void TransactionMetricsObserver::onAbortActive(ServerTransactionsMetrics* serverTransactionsMetrics,
                                               unsigned long long curTime,
                                               Top* top) {
    _onAbort(serverTransactionsMetrics, curTime, top);
    //
    // Per transaction metrics.
    //
    // The transaction operation may have already been aborted by another thread, so we check that
    // the transaction is active before setting it as inactive.
    if (_singleTransactionStats.isActive()) {
        _singleTransactionStats.setInactive(curTime);
    }

    //
    // Server wide transactions metrics.
    //
    serverTransactionsMetrics->decrementCurrentActive();
}

void TransactionMetricsObserver::onAbortInactive(
    ServerTransactionsMetrics* serverTransactionsMetrics, unsigned long long curTime, Top* top) {
    _onAbort(serverTransactionsMetrics, curTime, top);

    //
    // Server wide transactions metrics.
    //
    serverTransactionsMetrics->decrementCurrentInactive();
}

void TransactionMetricsObserver::onTransactionOperation(Client* client,
                                                        OpDebug::AdditiveMetrics additiveMetrics) {
    // Add the latest operation stats to the aggregate OpDebug::AdditiveMetrics object stored in the
    // SingleTransactionStats instance on the TransactionMetricsObserver.
    _singleTransactionStats.getOpDebug()->additiveMetrics.add(additiveMetrics);

    // Update the LastClientInfo object stored in the SingleTransactionStats instance on the
    // TransactionMetricsObserver with this Client's information. This is the last client that ran a
    // transaction operation on the txnParticipant.
    _singleTransactionStats.updateLastClientInfo(client);
}

void TransactionMetricsObserver::_onAbort(ServerTransactionsMetrics* serverTransactionsMetrics,
                                          unsigned long long curTime,
                                          Top* top) {
    //
    // Per transaction metrics.
    //
    _singleTransactionStats.setEndTime(curTime);

    //
    // Server wide transactions metrics.
    //
    serverTransactionsMetrics->incrementTotalAborted();
    serverTransactionsMetrics->decrementCurrentOpen();

    top->incrementGlobalTransactionLatencyStats(_singleTransactionStats.getDuration(curTime));
}

}  // namespace mongo
