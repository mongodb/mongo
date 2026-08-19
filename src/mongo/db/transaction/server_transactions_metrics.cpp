// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/transaction/server_transactions_metrics.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transaction/reclaimed_prepared_txn_tracker.h"
#include "mongo/db/transaction/retryable_writes_stats.h"
#include "mongo/db/transaction/transactions_stats_gen.h"
#include "mongo/util/decorable.h"

#include <mutex>
#include <utility>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {
const auto ServerTransactionsMetricsDecoration =
    ServiceContext::declareDecoration<ServerTransactionsMetrics>();
}  // namespace

ServerTransactionsMetrics* ServerTransactionsMetrics::get(ServiceContext* service) {
    return &ServerTransactionsMetricsDecoration(service);
}

ServerTransactionsMetrics* ServerTransactionsMetrics::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

unsigned long long ServerTransactionsMetrics::getCurrentActive() const {
    return _currentActive.loadRelaxed();
}

void ServerTransactionsMetrics::decrementCurrentActive() {
    _currentActive.fetchAndSubtractRelaxed(1);
}

void ServerTransactionsMetrics::incrementCurrentActive() {
    _currentActive.fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getCurrentInactive() const {
    return _currentInactive.loadRelaxed();
}

void ServerTransactionsMetrics::decrementCurrentInactive() {
    _currentInactive.fetchAndSubtractRelaxed(1);
}

void ServerTransactionsMetrics::incrementCurrentInactive() {
    _currentInactive.fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getCurrentOpen() const {
    return _currentOpen.loadRelaxed();
}

void ServerTransactionsMetrics::decrementCurrentOpen() {
    _currentOpen.fetchAndSubtractRelaxed(1);
}

void ServerTransactionsMetrics::incrementCurrentOpen() {
    _currentOpen.fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getTotalStarted() const {
    return _totalStartedInternal.loadRelaxed() + _totalStartedExternal.loadRelaxed();
}

unsigned long long ServerTransactionsMetrics::getTotalInternalStarted() const {
    return _totalStartedInternal.loadRelaxed();
}

unsigned long long ServerTransactionsMetrics::getTotalExternalStarted() const {
    return _totalStartedExternal.loadRelaxed();
}

void ServerTransactionsMetrics::incrementTotalStarted(bool isServerInitiated) {
    (isServerInitiated ? _totalStartedInternal : _totalStartedExternal).fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getTotalAborted() const {
    return _totalAbortedInternal.loadRelaxed() + _totalAbortedExternal.loadRelaxed();
}

unsigned long long ServerTransactionsMetrics::getTotalInternalAborted() const {
    return _totalAbortedInternal.loadRelaxed();
}

unsigned long long ServerTransactionsMetrics::getTotalExternalAborted() const {
    return _totalAbortedExternal.loadRelaxed();
}

void ServerTransactionsMetrics::incrementTotalAborted(bool isServerInitiated) {
    (isServerInitiated ? _totalAbortedInternal : _totalAbortedExternal).fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getTotalCommitted() const {
    return _totalCommittedInternal.loadRelaxed() + _totalCommittedExternal.loadRelaxed();
}

unsigned long long ServerTransactionsMetrics::getTotalInternalCommitted() const {
    return _totalCommittedInternal.loadRelaxed();
}

unsigned long long ServerTransactionsMetrics::getTotalExternalCommitted() const {
    return _totalCommittedExternal.loadRelaxed();
}

void ServerTransactionsMetrics::incrementTotalCommitted(bool isServerInitiated) {
    (isServerInitiated ? _totalCommittedInternal : _totalCommittedExternal).fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getTotalPrepared() const {
    return _totalPrepared.loadRelaxed();
}

void ServerTransactionsMetrics::incrementTotalPrepared() {
    _totalPrepared.fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getTotalPreparedThenCommitted() const {
    return _totalPreparedThenCommitted.loadRelaxed();
}

void ServerTransactionsMetrics::incrementTotalPreparedThenCommitted() {
    _totalPreparedThenCommitted.fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getTotalPreparedThenAborted() const {
    return _totalPreparedThenAborted.loadRelaxed();
}

void ServerTransactionsMetrics::incrementTotalPreparedThenAborted() {
    _totalPreparedThenAborted.fetchAndAddRelaxed(1);
}

unsigned long long ServerTransactionsMetrics::getCurrentPrepared() const {
    return _currentPrepared.loadRelaxed();
}

void ServerTransactionsMetrics::incrementCurrentPrepared() {
    _currentPrepared.fetchAndAddRelaxed(1);
}

void ServerTransactionsMetrics::decrementCurrentPrepared() {
    _currentPrepared.fetchAndSubtractRelaxed(1);
}

void ServerTransactionsMetrics::incrementReclaimedPreparedTxnsCommitted() {
    _reclaimedPreparedTxnsCommitted.fetchAndAddRelaxed(1);
}

long long ServerTransactionsMetrics::getReclaimedPreparedTxnsCommitted() const {
    return _reclaimedPreparedTxnsCommitted.loadRelaxed();
}

void ServerTransactionsMetrics::incrementReclaimedPreparedTxnsAborted() {
    _reclaimedPreparedTxnsAborted.fetchAndAddRelaxed(1);
}

long long ServerTransactionsMetrics::getReclaimedPreparedTxnsAborted() const {
    return _reclaimedPreparedTxnsAborted.loadRelaxed();
}

void ServerTransactionsMetrics::updateLastTransaction(size_t operationCount,
                                                      size_t oplogOperationBytes,
                                                      BSONObj writeConcern) {
    std::lock_guard<std::mutex> lg(_mutex);
    if (!_lastCommittedTransaction) {
        _lastCommittedTransaction = LastCommittedTransaction();
    }
    _lastCommittedTransaction->setOperationCount(operationCount);
    _lastCommittedTransaction->setOplogOperationBytes(oplogOperationBytes);
    _lastCommittedTransaction->setWriteConcern(std::move(writeConcern));
}

void ServerTransactionsMetrics::updateStats(TransactionsStats* stats, bool includeLastCommitted) {
    stats->setCurrentActive(_currentActive.loadRelaxed());
    stats->setCurrentInactive(_currentInactive.loadRelaxed());
    stats->setCurrentOpen(_currentOpen.loadRelaxed());
    stats->setTotalAborted(_totalAbortedInternal.loadRelaxed() +
                           _totalAbortedExternal.loadRelaxed());
    stats->setTotalAbortedInternal(_totalAbortedInternal.loadRelaxed());
    stats->setTotalAbortedExternal(_totalAbortedExternal.loadRelaxed());
    stats->setTotalCommitted(_totalCommittedInternal.loadRelaxed() +
                             _totalCommittedExternal.loadRelaxed());
    stats->setTotalCommittedInternal(_totalCommittedInternal.loadRelaxed());
    stats->setTotalCommittedExternal(_totalCommittedExternal.loadRelaxed());
    stats->setTotalStarted(_totalStartedInternal.loadRelaxed() +
                           _totalStartedExternal.loadRelaxed());
    stats->setTotalStartedInternal(_totalStartedInternal.loadRelaxed());
    stats->setTotalStartedExternal(_totalStartedExternal.loadRelaxed());
    stats->setTotalPrepared(_totalPrepared.loadRelaxed());
    stats->setTotalPreparedThenCommitted(_totalPreparedThenCommitted.loadRelaxed());
    stats->setTotalPreparedThenAborted(_totalPreparedThenAborted.loadRelaxed());
    stats->setCurrentPrepared(_currentPrepared.loadRelaxed());

    std::lock_guard<std::mutex> lg(_mutex);
    if (_lastCommittedTransaction && includeLastCommitted) {
        stats->setLastCommittedTransaction(*_lastCommittedTransaction);
    }
}

namespace {
class TransactionsSSS : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    ~TransactionsSSS() override = default;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        TransactionsStats stats;

        bool includeLastCommitted = true;
        if (configElement.type() == BSONType::object) {
            includeLastCommitted = configElement.Obj()["includeLastCommitted"].trueValue();
        }

        // Retryable writes and multi-document transactions metrics are both included in the same
        // serverStatus section because both utilize similar internal machinery for tracking their
        // lifecycle within a session. Both are assigned transaction numbers, and so both are often
        // referred to as “transactions”.
        RetryableWritesStats::get(opCtx)->updateStats(&stats);
        auto* serverTxnMetrics = ServerTransactionsMetrics::get(opCtx);
        serverTxnMetrics->updateStats(&stats, includeLastCommitted);

        auto* tracker = ReclaimedPreparedTxnTracker::get(opCtx);
        auto committed = serverTxnMetrics->getReclaimedPreparedTxnsCommitted();
        auto aborted = serverTxnMetrics->getReclaimedPreparedTxnsAborted();
        auto remaining = tracker->getNumReclaimedPreparedTxnsRemaining();
        if (committed + aborted + remaining > 0) {
            PreciseCheckpointRecoveryStats recoveryStats;
            recoveryStats.setNumReclaimedPreparedTxnsExitedPrepare(committed + aborted);
            recoveryStats.setNumReclaimedPreparedTxnsCommitted(committed);
            recoveryStats.setNumReclaimedPreparedTxnsAborted(aborted);
            recoveryStats.setNumReclaimedPreparedTxnsRemaining(remaining);
            recoveryStats.setRecoveryDurationMicros(tracker->getRecoveryDurationMicros());
            stats.setPreciseCheckpointRecovery(recoveryStats);
        }

        return stats.toBSON();
    }
};
auto& transactionsSSS = *ServerStatusSectionBuilder<TransactionsSSS>("transactions").forShard();
}  // namespace

}  // namespace mongo
