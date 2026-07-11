// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/transaction/transaction_metrics_observer.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/curop.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_stats.h"
#include "mongo/db/transaction/server_transactions_metrics.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

#include <cstdint>
#include <memory>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTransaction

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

void TransactionMetricsObserver::onStash(OperationContext* opCtx,
                                         ServerTransactionsMetrics* serverTransactionsMetrics,
                                         TickSource* tickSource) {
    //
    // Per transaction metrics.
    //
    if (!_singleTransactionStats.isActive()) {
        // Dump transaction info before hitting invariant.
        LOGV2(12336500,
              "Attempting to stash transaction metrics which were never unstashed",
              "lsid"_attr = opCtx->getLogicalSessionId(),
              "txnNumber"_attr = opCtx->getTxnNumber());
    }
    invariant(_singleTransactionStats.isActive());
    _singleTransactionStats.setInactive(tickSource, tickSource->getTicks());

    //
    // Server wide transactions metrics.
    //
    // We accept possible slight inaccuracies in these counters from non-atomicity.
    serverTransactionsMetrics->decrementCurrentActive();
    serverTransactionsMetrics->incrementCurrentInactive();
}

void TransactionMetricsObserver::onUnstash(OperationContext* opCtx,
                                           ServerTransactionsMetrics* serverTransactionsMetrics,
                                           TickSource* tickSource) {
    //
    // Per transaction metrics.
    //
    if (_singleTransactionStats.isActive()) {
        // Dump transaction info before hitting invariant.
        LOGV2(12336501,
              "Attempting to unstash metrics which are already active",
              "lsid"_attr = opCtx->getLogicalSessionId(),
              "txnNumber"_attr = opCtx->getTxnNumber());
    }
    invariant(!_singleTransactionStats.isActive());
    _singleTransactionStats.setActive(tickSource->getTicks());

    //
    // Server wide transactions metrics.
    //
    // We accept possible slight inaccuracies in these counters from non-atomicity.
    serverTransactionsMetrics->incrementCurrentActive();
    serverTransactionsMetrics->decrementCurrentInactive();
}

void TransactionMetricsObserver::onCommit(OperationContext* opCtx,
                                          ServerTransactionsMetrics* serverTransactionsMetrics,
                                          TickSource* tickSource,
                                          size_t operationCount,
                                          size_t oplogOperationBytes) {
    //
    // Per transaction metrics.
    //
    // After the transaction has been committed, we must update the end time and mark it as
    // inactive. We use the same "now" time to prevent skew in the time-related metrics.
    auto curTick = tickSource->getTicks();
    _singleTransactionStats.setEndTime(curTick);

    invariant(_singleTransactionStats.isActive());
    _singleTransactionStats.setInactive(tickSource, curTick);

    //
    // Server wide transactions metrics.
    //
    serverTransactionsMetrics->incrementTotalCommitted();
    serverTransactionsMetrics->decrementCurrentOpen();
    serverTransactionsMetrics->decrementCurrentActive();

    if (_singleTransactionStats.isPrepared()) {
        serverTransactionsMetrics->incrementTotalPreparedThenCommitted();
        serverTransactionsMetrics->decrementCurrentPrepared();
    }

    if (_singleTransactionStats.isRecoveredFromPreciseCheckpoint()) {
        serverTransactionsMetrics->incrementReclaimedPreparedTxnsCommitted();
    }

    serverTransactionsMetrics->updateLastTransaction(
        operationCount,
        oplogOperationBytes,
        opCtx->getWriteConcern().usedDefaultConstructedWC ? BSONObj()
                                                          : opCtx->getWriteConcern().toBSON());

    auto duration = _singleTransactionStats.getDuration(tickSource, curTick);
    ServiceLatencyTracker::getDecoration(opCtx->getService())
        .incrementForTransaction(opCtx, duration);
}

void TransactionMetricsObserver::_onAbortActive(
    OperationContext* opCtx,
    ServerTransactionsMetrics* serverTransactionsMetrics,
    TickSource* tickSource) {

    auto curTick = tickSource->getTicks();
    invariant(_singleTransactionStats.isActive());
    _onAbort(opCtx, serverTransactionsMetrics, curTick, tickSource);
    //
    // Per transaction metrics.
    //
    _singleTransactionStats.setInactive(tickSource, curTick);

    //
    // Server wide transactions metrics.
    //
    serverTransactionsMetrics->decrementCurrentActive();
}

void TransactionMetricsObserver::_onAbortInactive(
    OperationContext* opCtx,
    ServerTransactionsMetrics* serverTransactionsMetrics,
    TickSource* tickSource) {

    auto curTick = tickSource->getTicks();
    invariant(!_singleTransactionStats.isActive());
    _onAbort(opCtx, serverTransactionsMetrics, curTick, tickSource);

    //
    // Server wide transactions metrics.
    //
    serverTransactionsMetrics->decrementCurrentInactive();
}

void TransactionMetricsObserver::onAbort(OperationContext* opCtx,
                                         ServerTransactionsMetrics* serverTransactionsMetrics,
                                         TickSource* tickSource) {
    if (_singleTransactionStats.isActive()) {
        _onAbortActive(opCtx, serverTransactionsMetrics, tickSource);
    } else {
        _onAbortInactive(opCtx, serverTransactionsMetrics, tickSource);
    }
}

void TransactionMetricsObserver::onTransactionOperation(
    OperationContext* opCtx,
    OpDebug::AdditiveMetrics additiveMetrics,
    long long prepareReadConflicts,
    const SingleThreadedStorageMetrics& storageMetrics,
    bool isPrepared) {
    // Add the latest operation stats to the aggregate OpDebug::AdditiveMetrics and StorageMetrics
    // objects stored in the SingleTransactionStats instance on the TransactionMetricsObserver.
    _singleTransactionStats.getOpDebug()->getAdditiveMetrics().add(additiveMetrics);
    _singleTransactionStats.incrementPrepareReadConflicts(prepareReadConflicts);
    _singleTransactionStats.getTransactionStorageMetrics() += storageMetrics;

    // If there are valid storage statistics for this operation, put those in the
    // SingleTransactionStats instance either by creating a new storageStats instance or by adding
    // into an existing storageStats instance stored in SingleTransactionStats.
    // WiredTiger doesn't let storage statistics be collected when transaction is prepared.
    if (!isPrepared) {
        std::unique_ptr<StorageStats> storageStats =
            shard_role_details::getRecoveryUnit(opCtx)->computeOperationStatisticsSinceLastCall();
        if (storageStats) {
            if (!_singleTransactionStats.getOpDebug()->storageStats) {
                _singleTransactionStats.getOpDebug()->storageStats = storageStats->clone();
            } else {
                *_singleTransactionStats.getOpDebug()->storageStats += *storageStats;
            }
            CurOp::get(opCtx)->debug().storageStats = std::move(storageStats);
        }
    }

    // Update the LastClientInfo object stored in the SingleTransactionStats instance on the
    // TransactionMetricsObserver with this Client's information. This is the last client that ran a
    // transaction operation on the txnParticipant.
    _singleTransactionStats.updateLastClientInfo(opCtx->getClient());

    // Update TicketHolderQueueStats with the latest operation's queueing data.
    _singleTransactionStats.updateQueueStats(opCtx);
}

void TransactionMetricsObserver::_onAbort(OperationContext* opCtx,
                                          ServerTransactionsMetrics* serverTransactionsMetrics,
                                          TickSource::Tick curTick,
                                          TickSource* tickSource) {
    //
    // Per transaction metrics.
    //
    _singleTransactionStats.setEndTime(curTick);

    //
    // Server wide transactions metrics.
    //
    serverTransactionsMetrics->incrementTotalAborted();
    serverTransactionsMetrics->decrementCurrentOpen();

    if (_singleTransactionStats.isPrepared()) {
        serverTransactionsMetrics->incrementTotalPreparedThenAborted();
        serverTransactionsMetrics->decrementCurrentPrepared();
    }

    if (_singleTransactionStats.isRecoveredFromPreciseCheckpoint()) {
        serverTransactionsMetrics->incrementReclaimedPreparedTxnsAborted();
    }

    auto latency = _singleTransactionStats.getDuration(tickSource, curTick);
    ServiceLatencyTracker::getDecoration(opCtx->getService())
        .incrementForTransaction(opCtx, latency);
}

void TransactionMetricsObserver::onPrepare(ServerTransactionsMetrics* serverTransactionsMetrics,
                                           TickSource::Tick curTick) {
    //
    // Per transaction metrics.
    //
    _singleTransactionStats.setPreparedStartTime(curTick);

    serverTransactionsMetrics->incrementCurrentPrepared();
    serverTransactionsMetrics->incrementTotalPrepared();
}

}  // namespace mongo
