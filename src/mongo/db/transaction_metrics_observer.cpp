
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

#include "mongo/platform/basic.h"

#include "mongo/db/transaction_metrics_observer.h"

#include "mongo/db/server_transactions_metrics.h"
#include "mongo/db/transaction_participant.h"

namespace mongo {

void TransactionMetricsObserver::onStart(ServerTransactionsMetrics* serverTransactionsMetrics,
                                         bool isAutoCommit,
                                         TickSource* tickSource,
                                         Date_t curWallClockTime,
                                         Date_t expireDate) {
    //
    // Per transaction metrics.
    //
    _singleTransactionStats.setStartTime(tickSource->getTicks(), curWallClockTime);
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
                                         TickSource* tickSource) {
    //
    // Per transaction metrics.
    //
    // The transaction operation may be trying to stash its resources when it has already been
    // aborted by another thread, so we check that the transaction is active before setting it as
    // inactive.
    if (_singleTransactionStats.isActive()) {
        _singleTransactionStats.setInactive(tickSource, tickSource->getTicks());
    }

    //
    // Server wide transactions metrics.
    //
    // We accept possible slight inaccuracies in these counters from non-atomicity.
    serverTransactionsMetrics->decrementCurrentActive();
    serverTransactionsMetrics->incrementCurrentInactive();
}

void TransactionMetricsObserver::onUnstash(ServerTransactionsMetrics* serverTransactionsMetrics,
                                           TickSource* tickSource) {
    //
    // Per transaction metrics.
    //
    _singleTransactionStats.setActive(tickSource->getTicks());

    //
    // Server wide transactions metrics.
    //
    // We accept possible slight inaccuracies in these counters from non-atomicity.
    serverTransactionsMetrics->incrementCurrentActive();
    serverTransactionsMetrics->decrementCurrentInactive();
}

void TransactionMetricsObserver::onCommit(ServerTransactionsMetrics* serverTransactionsMetrics,
                                          TickSource* tickSource,
                                          boost::optional<repl::OpTime> oldestOplogEntryOpTime,
                                          boost::optional<repl::OpTime> commitOpTime,
                                          Top* top,
                                          bool wasPrepared) {
    invariant((oldestOplogEntryOpTime != boost::none && commitOpTime != boost::none) ||
              (oldestOplogEntryOpTime == boost::none && commitOpTime == boost::none));
    //
    // Per transaction metrics.
    //
    // After the transaction has been committed, we must update the end time and mark it as
    // inactive. We use the same "now" time to prevent skew in the time-related metrics.
    auto curTick = tickSource->getTicks();
    _singleTransactionStats.setEndTime(curTick);
    // The transaction operation may have already been aborted by another thread, so we check that
    // the transaction is active before setting it as inactive.
    if (_singleTransactionStats.isActive()) {
        _singleTransactionStats.setInactive(tickSource, curTick);
    }

    //
    // Server wide transactions metrics.
    //
    serverTransactionsMetrics->incrementTotalCommitted();
    serverTransactionsMetrics->decrementCurrentOpen();
    serverTransactionsMetrics->decrementCurrentActive();

    if (wasPrepared) {
        serverTransactionsMetrics->incrementTotalPreparedThenCommitted();
        serverTransactionsMetrics->decrementCurrentPrepared();
    }

    auto duration =
        durationCount<Microseconds>(_singleTransactionStats.getDuration(tickSource, curTick));
    top->incrementGlobalTransactionLatencyStats(static_cast<uint64_t>(duration));

    // Remove this transaction's oldest oplog entry OpTime if one was written.
    if (oldestOplogEntryOpTime) {
        serverTransactionsMetrics->removeActiveOpTime(*oldestOplogEntryOpTime, commitOpTime);
    }
}

void TransactionMetricsObserver::onAbortActive(ServerTransactionsMetrics* serverTransactionsMetrics,
                                               TickSource* tickSource,
                                               boost::optional<repl::OpTime> oldestOplogEntryOpTime,
                                               boost::optional<repl::OpTime> abortOpTime,
                                               Top* top,
                                               bool wasPrepared) {
    invariant((oldestOplogEntryOpTime != boost::none && abortOpTime != boost::none) ||
              (oldestOplogEntryOpTime == boost::none && abortOpTime == boost::none));

    auto curTick = tickSource->getTicks();
    _onAbort(serverTransactionsMetrics, curTick, tickSource, top);
    //
    // Per transaction metrics.
    //
    // The transaction operation may have already been aborted by another thread, so we check that
    // the transaction is active before setting it as inactive.
    if (_singleTransactionStats.isActive()) {
        _singleTransactionStats.setInactive(tickSource, curTick);
    }

    //
    // Server wide transactions metrics.
    //
    serverTransactionsMetrics->decrementCurrentActive();

    if (wasPrepared) {
        serverTransactionsMetrics->incrementTotalPreparedThenAborted();
        serverTransactionsMetrics->decrementCurrentPrepared();
    }

    // Remove this transaction's oldest oplog entry OpTime if one was written.
    if (oldestOplogEntryOpTime) {
        serverTransactionsMetrics->removeActiveOpTime(*oldestOplogEntryOpTime, abortOpTime);
    }
}

void TransactionMetricsObserver::onAbortInactive(
    ServerTransactionsMetrics* serverTransactionsMetrics,
    TickSource* tickSource,
    boost::optional<repl::OpTime> oldestOplogEntryOpTime,
    Top* top) {
    auto curTick = tickSource->getTicks();
    _onAbort(serverTransactionsMetrics, curTick, tickSource, top);

    //
    // Server wide transactions metrics.
    //
    serverTransactionsMetrics->decrementCurrentInactive();

    // Remove this transaction's oldest oplog entry OpTime if one was written.
    if (oldestOplogEntryOpTime) {
        serverTransactionsMetrics->removeActiveOpTime(*oldestOplogEntryOpTime, boost::none);
    }
}

void TransactionMetricsObserver::onTransactionOperation(
    Client* client,
    OpDebug::AdditiveMetrics additiveMetrics,
    std::shared_ptr<StorageStats> storageStats) {
    // Add the latest operation stats to the aggregate OpDebug::AdditiveMetrics object stored in the
    // SingleTransactionStats instance on the TransactionMetricsObserver.
    _singleTransactionStats.getOpDebug()->additiveMetrics.add(additiveMetrics);

    // If there are valid storage statistics for this operation, put those in the
    // SingleTransactionStats instance either by creating a new storageStats instance or by adding
    // into an existing storageStats instance stored in SingleTransactionStats.
    if (storageStats) {
        if (!_singleTransactionStats.getOpDebug()->storageStats) {
            _singleTransactionStats.getOpDebug()->storageStats = storageStats->getCopy();
        } else {
            *_singleTransactionStats.getOpDebug()->storageStats += *storageStats;
        }
    }

    // Update the LastClientInfo object stored in the SingleTransactionStats instance on the
    // TransactionMetricsObserver with this Client's information. This is the last client that ran a
    // transaction operation on the txnParticipant.
    _singleTransactionStats.updateLastClientInfo(client);
}

void TransactionMetricsObserver::_onAbort(ServerTransactionsMetrics* serverTransactionsMetrics,
                                          TickSource::Tick curTick,
                                          TickSource* tickSource,
                                          Top* top) {
    //
    // Per transaction metrics.
    //
    _singleTransactionStats.setEndTime(curTick);

    //
    // Server wide transactions metrics.
    //
    serverTransactionsMetrics->incrementTotalAborted();
    serverTransactionsMetrics->decrementCurrentOpen();

    auto latency =
        durationCount<Microseconds>(_singleTransactionStats.getDuration(tickSource, curTick));
    top->incrementGlobalTransactionLatencyStats(static_cast<uint64_t>(latency));
}

void TransactionMetricsObserver::onPrepare(ServerTransactionsMetrics* serverTransactionsMetrics,
                                           repl::OpTime prepareOpTime,
                                           TickSource::Tick curTick) {

    //
    // Per transaction metrics.
    //
    _singleTransactionStats.setPreparedStartTime(curTick);

    // Since we currently only write an oplog entry for an in progress transaction when it is in
    // the prepare state, the prepareOpTime is currently the oldest OpTime written to the
    // oplog for this transaction.
    serverTransactionsMetrics->addActiveOpTime(prepareOpTime);
    serverTransactionsMetrics->incrementCurrentPrepared();
    serverTransactionsMetrics->incrementTotalPrepared();
}

}  // namespace mongo
